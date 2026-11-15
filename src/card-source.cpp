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

// Источник «Сейчас играет»: внутри держит скрытый (private) источник obs-browser и рисует его.
// Пользователю не нужно знать URL и размеры — они задаются темой в свойствах источника.

#include "card-source.hpp"
#include "runtime.hpp"

#include <obs-module.h>
#include <util/platform.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif
#include <plugin-support.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <utility>
#include <string>

namespace trackspin {
namespace {

constexpr const char *kSourceId = "trackspin_card";
constexpr const char *kThemeCompact = "compact";
constexpr const char *kThemeVinyl = "vinyl";

// Внешний вид карточки — всё, что уходит в адрес страницы виджета.
struct Look {
	std::string theme = kThemeCompact;
	std::string layout = "horizontal";
	std::string bg = "dark";
	std::string font = "system";
	std::string accent; // RRGGBB или пусто — цвет темы
	std::string time = "off"; // время трека: off|elapsed|remaining|elapsed_total|elapsed_remaining
	int scale = 200; // страница рисуется в 2× (карточка остаётся чёткой при увеличении рамкой в сцене)
	int show_for = 0; // секунд после смены трека; 0 — показывать всегда
	bool hide_on_pause = false;
	bool show_progress = true;
};

struct ThemeSize {
	int width;
	int height;
};

// Размер страницы = карточка + отступ 12 px со всех сторон (см. web/widget/style.css), умноженный на масштаб.
ThemeSize look_size(const Look &look)
{
	const bool vinyl = look.theme == kThemeVinyl;
	ThemeSize s = look.layout == "vertical" ? (vinyl ? ThemeSize{312, 408} : ThemeSize{256, 336})
						 : (vinyl ? ThemeSize{600, 200} : ThemeSize{500, 140});
	if (look.layout == "vertical" && look.time != "off")
		s.height += 20; // строка времени под полосой
	s.width = (s.width * look.scale + 50) / 100;
	s.height = (s.height * look.scale + 50) / 100;
	return s;
}

// Цвет OBS хранится как 0xAABBGGRR.
std::string color_hex(long long c)
{
	char buf[8];
	snprintf(buf, sizeof(buf), "%02x%02x%02x", (unsigned)(c & 0xff), (unsigned)((c >> 8) & 0xff),
		 (unsigned)((c >> 16) & 0xff));
	return buf;
}

std::string percent_encode(const std::string &s)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		    c == '.' || c == '~') {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0xF]);
		}
	}
	return out;
}

// Карточку отдаёт встроенный сервер; если он не запустился — страница с понятной ошибкой.
std::string widget_url(const Look &look)
{
	std::string base = runtime_base_url();
	if (!base.empty()) {
		std::string url = base + "/widget?theme=" + look.theme;
		if (look.layout != "horizontal")
			url += "&layout=" + look.layout;
		if (look.bg != "dark")
			url += "&bg=" + look.bg;
		if (look.font != "system")
			url += "&font=" + look.font;
		if (!look.accent.empty())
			url += "&accent=" + look.accent;
		if (look.scale != 100)
			url += "&scale=" + std::to_string(look.scale / 100) + "." + (look.scale % 100 < 10 ? "0" : "") +
			       std::to_string(look.scale % 100);
		if (look.time != "off")
			url += "&time=" + look.time;
		if (look.show_for > 0)
			url += "&showFor=" + std::to_string(look.show_for);
		if (look.hide_on_pause)
			url += "&hideOnPause=1";
		if (!look.show_progress)
			url += "&progress=0";
		return url;
	}
	std::string html =
		"<!doctype html><meta charset=utf-8><style>html,body{margin:0;background:transparent;"
		"font:600 18px 'Segoe UI',sans-serif;color:#fff}div{margin:12px;padding:18px 22px;border-radius:16px;"
		"background:rgba(150,30,30,.85)}</style><div>Trackspin: the background program did not start "
		"(agent is not running). See OBS log.</div>";
	return "data:text/html;charset=utf-8," + percent_encode(html);
}

struct Card {
	obs_source_t *self = nullptr;
	obs_source_t *browser = nullptr;
	Look look;
	std::string applied_base;
	std::string position = "manual"; // автовыравнивание в сцене: manual или top_left…bottom_right
	int margin = 40;                 // отступ от края сцены, px
	float align_timer = 0.0f; // адрес агента, с которым открыта карточка
};

void apply_browser_settings(Card *card)
{
	if (!card->browser)
		return;
	ThemeSize size = look_size(card->look);
	card->applied_base = runtime_base_url();
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "is_local_file", false);
	obs_data_set_string(s, "url", widget_url(card->look).c_str());
	obs_data_set_int(s, "width", size.width);
	obs_data_set_int(s, "height", size.height);
	obs_data_set_bool(s, "shutdown", false);
	obs_data_set_bool(s, "restart_when_active", false);
	obs_data_set_bool(s, "reroute_audio", false);
	obs_source_update(card->browser, s);
	obs_data_release(s);
}

const char *card_get_name(void *)
{
	return obs_module_text("SourceName");
}

void card_update(void *data, obs_data_t *settings)
{
	auto *card = static_cast<Card *>(data);
	auto str = [&](const char *key, const char *fallback) {
		const char *v = obs_data_get_string(settings, key);
		return std::string((v && *v) ? v : fallback);
	};
	Look &look = card->look;
	look.theme = str("theme", kThemeCompact);
	look.layout = str("layout", "horizontal");
	look.bg = str("background", "dark");
	look.font = str("font", "system");
	look.accent = obs_data_get_bool(settings, "accent_custom") ? color_hex(obs_data_get_int(settings, "accent"))
								   : std::string();
	look.time = str("time_format", "off");
	// Размер меняют рамкой источника в сцене, как у любого источника OBS; scale — внутреннее разрешение.
	look.hide_on_pause = str("pause_mode", "dim") == "hide";
	look.show_for = str("show_mode", "always") == "track" ? (int)obs_data_get_int(settings, "show_seconds") : 0;
	card->position = str("position", "manual");
	card->margin = (int)obs_data_get_int(settings, "position_margin");
	card->align_timer = 1.0f; // выровнять на ближайшем кадре
	look.show_progress = obs_data_get_bool(settings, "show_progress");
	apply_browser_settings(card);

	RuntimeOptions opts;
	opts.yandex = obs_data_get_bool(settings, "svc_yandex");
	opts.vk = obs_data_get_bool(settings, "svc_vk");
	opts.spotify = obs_data_get_bool(settings, "svc_spotify");
	opts.appleMusic = obs_data_get_bool(settings, "svc_apple_music");
	opts.youtubeMusic = obs_data_get_bool(settings, "svc_youtube_music");
	opts.soundcloud = obs_data_get_bool(settings, "svc_soundcloud");
	opts.deezer = obs_data_get_bool(settings, "svc_deezer");
	opts.tidal = obs_data_get_bool(settings, "svc_tidal");
	opts.amazonMusic = obs_data_get_bool(settings, "svc_amazon_music");
	opts.youtube = obs_data_get_bool(settings, "svc_youtube");
	opts.otherPlayers = obs_data_get_bool(settings, "other_players");
	opts.videoPlayers = obs_data_get_bool(settings, "video_players");
	opts.browser_without_extension = obs_data_get_bool(settings, "browser_smtc");
	opts.demo = obs_data_get_bool(settings, "demo");
	// «Не показывать»: каждая строка — исполнитель, название или «Исполнитель - Название».
	std::string text = obs_data_get_string(settings, "blocklist_text");
	for (size_t pos = 0; pos <= text.size();) {
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		if (!line.empty())
			opts.blocklist.push_back(line);
		if (nl == std::string::npos)
			break;
		pos = nl + 1;
	}
	// Записи из прежнего списка-виджета (версии до этой) тоже учитываем.
	obs_data_array_t *bl = obs_data_get_array(settings, "blocklist");
	for (size_t i = 0, n = bl ? obs_data_array_count(bl) : 0; i < n; ++i) {
		obs_data_t *item = obs_data_array_item(bl, i);
		const char *v = obs_data_get_string(item, "value");
		if (v && *v)
			opts.blocklist.push_back(v);
		obs_data_release(item);
	}
	obs_data_array_release(bl);
	if (obs_data_get_bool(settings, "text_files"))
		opts.output_dir = obs_data_get_string(settings, "text_files_dir");
	runtime_set_options(opts);
}

void *card_create(obs_data_t *settings, obs_source_t *source)
{
	auto *card = new Card();
	card->self = source;
	card->browser = obs_source_create_private("browser_source", "trackspin-browser", nullptr);
	if (!card->browser)
		obs_log(LOG_WARNING, "browser_source is not available: the card cannot be displayed");
	card_update(card, settings);
	return card;
}

void card_destroy(void *data)
{
	auto *card = static_cast<Card *>(data);
	if (card->browser)
		obs_source_release(card->browser);
	delete card;
}

void card_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "theme", kThemeCompact);
	obs_data_set_default_string(settings, "layout", "horizontal");
	obs_data_set_default_string(settings, "background", "dark");
	obs_data_set_default_string(settings, "font", "system");
	obs_data_set_default_bool(settings, "accent_custom", false);
	obs_data_set_default_int(settings, "accent", 0xff2b4ef0); // коралловый #F04E2B
	obs_data_set_default_string(settings, "time_format", "off");
	obs_data_set_default_bool(settings, "text_files", false);
	if (char *dir = obs_module_config_path("now-playing")) {
		obs_data_set_default_string(settings, "text_files_dir", dir);
		bfree(dir);
	}
	obs_data_set_default_string(settings, "show_mode", "always");
	obs_data_set_default_int(settings, "show_seconds", 10);
	obs_data_set_default_string(settings, "position", "manual");
	obs_data_set_default_int(settings, "position_margin", 40);
	obs_data_set_default_string(settings, "pause_mode", "dim");
	obs_data_set_default_bool(settings, "show_progress", true);
	obs_data_set_default_bool(settings, "svc_yandex", true);
	obs_data_set_default_bool(settings, "svc_vk", true);
	obs_data_set_default_bool(settings, "svc_spotify", true);
	obs_data_set_default_bool(settings, "svc_apple_music", true);
	obs_data_set_default_bool(settings, "svc_youtube_music", true);
	obs_data_set_default_bool(settings, "svc_soundcloud", true);
	obs_data_set_default_bool(settings, "svc_deezer", true);
	obs_data_set_default_bool(settings, "svc_tidal", true);
	obs_data_set_default_bool(settings, "svc_amazon_music", true);
	obs_data_set_default_bool(settings, "svc_youtube", false);
	obs_data_set_default_bool(settings, "other_players", true);
	obs_data_set_default_bool(settings, "video_players", false);
	obs_data_set_default_bool(settings, "browser_smtc", true);
	obs_data_set_default_bool(settings, "demo", false);
}

const char *service_name(const char *s)
{
	static const struct {
		const char *id, *key;
	} names[] = {{"yandex", "Service.Yandex"},        {"vk", "Service.Vk"},
		     {"spotify", "Service.Spotify"},      {"apple-music", "Service.AppleMusic"},
		     {"youtube-music", "Service.YouTubeMusic"}, {"youtube", "Service.YouTube"},
		     {"soundcloud", "Service.SoundCloud"}, {"deezer", "Service.Deezer"},
		     {"tidal", "Service.Tidal"},          {"amazon-music", "Service.AmazonMusic"}};
	for (const auto &n : names)
		if (!strcmp(s, n.id))
			return obs_module_text(n.key);
	return obs_module_text("Service.Unknown");
}

// Имя для статуса: у «других» плееров — их собственное имя (AIMP, VLC…).
std::string source_name(obs_data_t *d)
{
	const char *svc = obs_data_get_string(d, "service");
	const char *player = obs_data_get_string(d, "player");
	if (!strcmp(svc, "other") && player && *player)
		return player;
	return service_name(svc);
}

const char *browser_name(const char *app)
{
	static const struct {
		const char *id, *name;
	} names[] = {{"chrome", "Chrome"},   {"edge", "Edge"},       {"firefox", "Firefox"},
		     {"opera", "Opera"},     {"yandex-browser", "Yandex Browser"}};
	for (const auto &n : names)
		if (!strcmp(app, n.id))
			return n.name;
	return app;
}

// Состояние для окна свойств: что показывает карточка и какие плееры подключены (спрашиваем у агента).
struct Status {
	enum Level { Ok, Idle, Down } level = Ok;
	std::string now;
	std::string sources;
};

Status read_status()
{
	Status st;
	std::string health = runtime_get("/api/health");
	if (health.empty()) {
		st.level = Status::Down;
		st.now = obs_module_text("Status.AgentDown");
		st.sources = "—";
		return st;
	}
	obs_data_t *np = obs_data_create_from_json(runtime_get("/api/now-playing").c_str());
	if (np && obs_data_get_bool(np, "active")) {
		st.now = std::string(obs_data_get_string(np, "artist")) + " — " + obs_data_get_string(np, "title") +
			 " · " + source_name(np);
	} else {
		st.level = Status::Idle;
		st.now = obs_module_text("Status.Nothing");
	}
	obs_data_release(np);

	obs_data_t *h = obs_data_create_from_json(health.c_str());
	if (st.level == Status::Idle && h) {
		if (obs_data_t *hid = obs_data_get_obj(h, "hidden")) {
			st.now = std::string(obs_module_text("Status.Hidden")) + ": " + obs_data_get_string(hid, "artist") +
				 " — " + obs_data_get_string(hid, "title");
			obs_data_release(hid);
		}
	}
	obs_data_array_t *arr = h ? obs_data_get_array(h, "sources") : nullptr;
	std::string list;
	for (size_t i = 0, n = arr ? obs_data_array_count(arr) : 0; i < n; ++i) {
		obs_data_t *src = obs_data_array_item(arr, i);
		const char *kind = obs_data_get_string(src, "kind");
		const char *app = obs_data_get_string(src, "app");
		std::string item;
		if (!strcmp(kind, "smtc-app"))
			item = source_name(src) + " (" + obs_module_text("Status.App") + ")";
		else if (!strcmp(kind, "extension"))
			item = std::string(browser_name(app)) + " (" + obs_module_text("Status.Extension") + ")";
		else if (!strcmp(kind, "smtc-browser"))
			item = std::string(browser_name(app)) + " (" + obs_module_text("Status.NoExtension") + ")";
		else
			item = obs_module_text("Status.Demo");
		if (list.find(item) == std::string::npos)
			list += (list.empty() ? "" : ", ") + item;
		obs_data_release(src);
	}
	obs_data_array_release(arr);
	obs_data_release(h);
	st.sources = list.empty() ? obs_module_text("Status.None") : list;
	return st;
}

// Текст статуса живёт в settings источника: у OBS_TEXT_INFO подпись слева видна, только когда значение непустое.
// card_update эти ключи не читает.
void put_status(Card *card, obs_properties_t *props)
{
	Status st = read_status();
	obs_data_t *s = obs_source_get_settings(card->self);
	obs_data_set_string(s, "status_now", st.now.c_str());
	obs_data_set_string(s, "status_sources", st.sources.c_str());
	obs_data_release(s);
	enum obs_text_info_type type = st.level == Status::Down   ? OBS_TEXT_INFO_ERROR
				       : st.level == Status::Idle ? OBS_TEXT_INFO_WARNING
								  : OBS_TEXT_INFO_NORMAL;
	if (props) {
		obs_property_text_set_info_type(obs_properties_get(props, "status_now"), type);
		obs_property_text_set_info_type(obs_properties_get(props, "status_sources"),
						st.level == Status::Down ? OBS_TEXT_INFO_ERROR : OBS_TEXT_INFO_NORMAL);
	}
}

bool status_refresh_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	put_status(static_cast<Card *>(data), props);
	return true;
}

// Последние 10 треков: «21:04  Исполнитель — Название». Пусто, если агент не отвечает или истории нет.
std::string html_escape(const std::string &s)
{
	std::string out;
	for (char c : s) {
		if (c == '<')
			out += "&lt;";
		else if (c == '>')
			out += "&gt;";
		else if (c == '&')
			out += "&amp;";
		else
			out += c;
	}
	return out;
}

std::string history_html()
{
	std::string body = runtime_get("/api/history");
	if (body.empty() || body == "[]")
		return {};
	obs_data_t *root = obs_data_create_from_json(("{\"items\":" + body + "}").c_str());
	obs_data_array_t *arr = root ? obs_data_get_array(root, "items") : nullptr;
	std::string html;
	for (size_t i = 0, n = arr ? std::min<size_t>(obs_data_array_count(arr), 10) : 0; i < n; ++i) {
		obs_data_t *it = obs_data_array_item(arr, i);
		time_t t = (time_t)(obs_data_get_int(it, "startedAt") / 1000);
		char hhmm[8] = "";
		if (struct tm *lt = localtime(&t))
			strftime(hhmm, sizeof(hhmm), "%H:%M", lt);
		html += std::string(i ? "<br>" : "") + "<span style='color:#8a8f9c'>" + hhmm + "</span>&nbsp;&nbsp;" +
			html_escape(obs_data_get_string(it, "artist")) + " — " + html_escape(obs_data_get_string(it, "title"));
		obs_data_release(it);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
	return html;
}

// Шапка: логотип, имя, версия и ссылки (QLabel понимает простой HTML).
std::string about_html()
{
	std::string logo;
	if (char *path = obs_module_file("header-logo.png")) {
		logo = path;
		bfree(path);
		for (char &c : logo)
			if (c == '\\')
				c = '/';
	}
	std::string html = "<table cellspacing=0 cellpadding=0><tr>";
	if (!logo.empty())
		html += "<td style='padding-right:10px'><img src='" + logo + "' width=40 height=40></td>";
	html += "<td><b style='font-size:15px'>Trackspin</b> " + std::string(PLUGIN_VERSION) + "<br><a href='" +
		obs_module_text("Link.SiteUrl") + "'>trackspin.app</a> &nbsp;·&nbsp; <a href='" +
		obs_module_text("Link.HelpUrl") + "'>" + obs_module_text("Link.Help") + "</a></td></tr></table>";
	return html;
}

obs_property_t *add_list(obs_properties_t *props, const char *key, const char *label,
			 std::initializer_list<std::pair<const char *, const char *>> items)
{
	obs_property_t *p = obs_properties_add_list(props, key, obs_module_text(label), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	for (const auto &it : items)
		obs_property_list_add_string(p, obs_module_text(it.first), it.second);
	return p;
}

// «Свой цвет полосы» имеет смысл, только когда полоса включена.
bool bar_options_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool bar = obs_data_get_bool(settings, "show_progress");
	obs_property_set_visible(obs_properties_get(props, "accent_custom"), bar);
	obs_property_set_visible(obs_properties_get(props, "accent"), bar && obs_data_get_bool(settings, "accent_custom"));
	return true;
}

// Открыть папку с файлами в Проводнике (создаём, если её ещё нет).
bool open_folder_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *card = static_cast<Card *>(data);
	if (!card)
		return false;
	obs_data_t *s = obs_source_get_settings(card->self);
	std::string dir = obs_data_get_string(s, "text_files_dir");
	obs_data_release(s);
	if (dir.empty())
		return false;
	os_mkdirs(dir.c_str());
#ifdef _WIN32
	wchar_t *wdir = nullptr;
	if (os_utf8_to_wcs_ptr(dir.c_str(), 0, &wdir)) {
		ShellExecuteW(nullptr, L"open", wdir, nullptr, nullptr, SW_SHOWNORMAL);
		bfree(wdir);
	}
#endif
	return false;
}

bool behavior_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_set_visible(obs_properties_get(props, "show_seconds"),
				 strcmp(obs_data_get_string(settings, "show_mode"), "track") == 0);
	obs_property_set_visible(obs_properties_get(props, "position_margin"),
				 strcmp(obs_data_get_string(settings, "position"), "manual") != 0);
	return true;
}

obs_properties_t *card_properties(void *data)
{
	auto *card = static_cast<Card *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *about = obs_properties_add_text(props, "about", about_html().c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(about, false);

	obs_properties_t *status = obs_properties_create();
	obs_property_text_set_info_word_wrap(
		obs_properties_add_text(status, "status_now", obs_module_text("Status.Now"), OBS_TEXT_INFO), true);
	obs_property_text_set_info_word_wrap(
		obs_properties_add_text(status, "status_sources", obs_module_text("Status.Connected"), OBS_TEXT_INFO),
		true);
	obs_properties_add_button2(status, "status_refresh", obs_module_text("Status.Refresh"), status_refresh_clicked,
				   card);
	obs_properties_add_group(props, "grp_status", obs_module_text("Group.Status"), OBS_GROUP_NORMAL, status);

	std::string history = history_html();
	if (!history.empty()) {
		obs_properties_t *hist = obs_properties_create();
		obs_property_text_set_info_word_wrap(
			obs_properties_add_text(hist, "history_info", history.c_str(), OBS_TEXT_INFO), true);
		obs_properties_add_group(props, "grp_history", obs_module_text("Group.History"), OBS_GROUP_NORMAL, hist);
	}
	if (card)
		put_status(card, props);

	obs_properties_t *look = obs_properties_create();
	add_list(look, "theme", "Theme", {{"Theme.Compact", kThemeCompact}, {"Theme.Vinyl", kThemeVinyl}});
	add_list(look, "layout", "Layout", {{"Layout.Horizontal", "horizontal"}, {"Layout.Vertical", "vertical"}});
	add_list(look, "background", "Background",
		 {{"Background.Dark", "dark"}, {"Background.Light", "light"}, {"Background.Cover", "cover"}});
	add_list(look, "font", "Font",
		 {{"Font.System", "system"}, {"Font.Onest", "onest"}, {"Font.Unbounded", "unbounded"}});
	obs_properties_add_group(props, "grp_look", obs_module_text("Group.Look"), OBS_GROUP_NORMAL, look);

	obs_properties_t *time = obs_properties_create();
	obs_property_t *bar = obs_properties_add_bool(time, "show_progress", obs_module_text("ShowProgress"));
	obs_property_set_modified_callback(bar, bar_options_changed);
	add_list(time, "time_format", "TimeFormat",
		 {{"TimeFormat.Off", "off"},
		  {"TimeFormat.Elapsed", "elapsed"},
		  {"TimeFormat.Remaining", "remaining"},
		  {"TimeFormat.ElapsedTotal", "elapsed_total"},
		  {"TimeFormat.ElapsedRemaining", "elapsed_remaining"}});
	obs_property_t *ac = obs_properties_add_bool(time, "accent_custom", obs_module_text("AccentCustom"));
	obs_property_set_modified_callback(ac, bar_options_changed);
	obs_properties_add_color(time, "accent", obs_module_text("Accent"));
	obs_properties_add_group(props, "grp_time", obs_module_text("Group.Time"), OBS_GROUP_NORMAL, time);

	obs_properties_t *behavior = obs_properties_create();
	obs_property_t *pause = obs_properties_add_list(behavior, "pause_mode", obs_module_text("PauseMode"),
							OBS_COMBO_TYPE_RADIO, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(pause, obs_module_text("PauseMode.Dim"), "dim");
	obs_property_list_add_string(pause, obs_module_text("PauseMode.Hide"), "hide");
	obs_property_t *show = add_list(behavior, "show_mode", "ShowMode",
					{{"ShowMode.Always", "always"}, {"ShowMode.Track", "track"}});
	obs_property_set_modified_callback(show, behavior_changed);
	obs_property_t *secs =
		obs_properties_add_int(behavior, "show_seconds", obs_module_text("ShowSeconds"), 3, 120, 1);
	obs_property_int_set_suffix(secs, obs_module_text("Unit.Seconds"));
	obs_property_t *bl =
		obs_properties_add_text(behavior, "blocklist_text", obs_module_text("Blocklist"), OBS_TEXT_MULTILINE);
	obs_property_set_long_description(bl, obs_module_text("Blocklist.Description"));
	std::string bl_help = std::string("<a href='") + obs_module_text("Link.HelpUrl") + "#blocklist'>" +
			      obs_module_text("Blocklist.Help") + "</a>";
	obs_properties_add_text(behavior, "blocklist_help", bl_help.c_str(), OBS_TEXT_INFO);
	obs_property_t *pos = add_list(behavior, "position", "Position",
				       {{"Position.Manual", "manual"},
					{"Position.TopLeft", "top_left"},
					{"Position.TopCenter", "top_center"},
					{"Position.TopRight", "top_right"},
					{"Position.MiddleLeft", "middle_left"},
					{"Position.Center", "center"},
					{"Position.MiddleRight", "middle_right"},
					{"Position.BottomLeft", "bottom_left"},
					{"Position.BottomCenter", "bottom_center"},
					{"Position.BottomRight", "bottom_right"}});
	obs_property_set_long_description(pos, obs_module_text("Position.Description"));
	obs_property_set_modified_callback(pos, behavior_changed);
	obs_property_t *margin =
		obs_properties_add_int(behavior, "position_margin", obs_module_text("PositionMargin"), 0, 400, 4);
	obs_property_int_set_suffix(margin, " px");
	obs_properties_add_group(props, "grp_behavior", obs_module_text("Group.Behavior"), OBS_GROUP_NORMAL, behavior);

	obs_properties_t *services = obs_properties_create();
	static const char *const svc[][2] = {{"svc_yandex", "Service.Yandex"},
					      {"svc_vk", "Service.Vk"},
					      {"svc_spotify", "Service.Spotify"},
					      {"svc_youtube_music", "Service.YouTubeMusic"},
					      {"svc_apple_music", "Service.AppleMusic"},
					      {"svc_soundcloud", "Service.SoundCloud"},
					      {"svc_deezer", "Service.Deezer"},
					      {"svc_tidal", "Service.Tidal"},
					      {"svc_amazon_music", "Service.AmazonMusic"}};
	for (const auto &s : svc)
		obs_properties_add_bool(services, s[0], obs_module_text(s[1]));
	obs_properties_add_group(props, "grp_services", obs_module_text("Group.Services"), OBS_GROUP_NORMAL, services);

	obs_properties_t *other = obs_properties_create();
	obs_properties_add_bool(other, "other_players", obs_module_text("OtherPlayers"));
	obs_properties_add_bool(other, "video_players", obs_module_text("VideoPlayers"));
	obs_properties_add_bool(other, "svc_youtube", obs_module_text("Service.YouTubeVideos"));
	obs_property_t *br = obs_properties_add_bool(other, "browser_smtc", obs_module_text("BrowserSmtc"));
	obs_property_set_long_description(br, obs_module_text("BrowserSmtc.Description"));
	obs_property_t *demo = obs_properties_add_bool(other, "demo", obs_module_text("Demo"));
	obs_property_set_long_description(demo, obs_module_text("Demo.Description"));
	obs_properties_add_group(props, "grp_other", obs_module_text("Group.Other"), OBS_GROUP_NORMAL, other);

	// Файлы и API для других программ: текстовые источники OBS, боты, свои виджеты.
	obs_properties_t *files = obs_properties_create();
	obs_properties_add_path(files, "text_files_dir", obs_module_text("TextFiles.Dir"), OBS_PATH_DIRECTORY, nullptr,
				nullptr);
	obs_properties_add_button2(files, "text_files_open", obs_module_text("TextFiles.Open"), open_folder_clicked,
				   card);
	obs_property_t *files_info =
		obs_properties_add_text(files, "text_files_info", obs_module_text("TextFiles.Info"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(files_info, true);
	obs_properties_add_group(props, "text_files", obs_module_text("TextFiles"), OBS_GROUP_CHECKABLE, files);

	std::string base = runtime_base_url();
	if (!base.empty()) {
		std::string api = std::string(obs_module_text("Api.Info")) + "<br><a href='" + base +
				  "/api/now-playing'>" + base + "/api/now-playing</a> &nbsp;·&nbsp; " + base + "/events";
		obs_property_text_set_info_word_wrap(obs_properties_add_text(props, "api_info", api.c_str(), OBS_TEXT_INFO),
						     true);
	}
	return props;
}

// OBS видит карточку в обычном размере, а браузер внутри рисует её в look.scale/100 раз крупнее.
uint32_t card_width(void *data)
{
	auto *card = static_cast<Card *>(data);
	return card->browser ? obs_source_get_width(card->browser) * 100 / card->look.scale : 0;
}

uint32_t card_height(void *data)
{
	auto *card = static_cast<Card *>(data);
	return card->browser ? obs_source_get_height(card->browser) * 100 / card->look.scale : 0;
}

void card_render(void *data, gs_effect_t *)
{
	auto *card = static_cast<Card *>(data);
	if (!card->browser)
		return;
	const float k = 100.0f / (float)card->look.scale;
	gs_matrix_push();
	gs_matrix_scale3f(k, k, 1.0f);
	obs_source_video_render(card->browser);
	gs_matrix_pop();
}

// Автовыравнивание: ставим точку привязки элемента сцены в нужный угол/край, позицию — с отступом от края.
// Карточка растёт от своей точки привязки, поэтому при смене темы остаётся на месте.
struct AlignTarget {
	uint32_t align;
	float x, y;
};

bool position_target(const std::string &pos, float w, float h, float m, AlignTarget &t)
{
	const float cx = w / 2, cy = h / 2;
	static const struct {
		const char *id;
		uint32_t align;
		int hx, vy; // -1 край слева/сверху, 0 центр, 1 справа/снизу
	} table[] = {{"top_left", OBS_ALIGN_TOP | OBS_ALIGN_LEFT, -1, -1},
		     {"top_center", OBS_ALIGN_TOP, 0, -1},
		     {"top_right", OBS_ALIGN_TOP | OBS_ALIGN_RIGHT, 1, -1},
		     {"middle_left", OBS_ALIGN_LEFT, -1, 0},
		     {"center", OBS_ALIGN_CENTER, 0, 0},
		     {"middle_right", OBS_ALIGN_RIGHT, 1, 0},
		     {"bottom_left", OBS_ALIGN_BOTTOM | OBS_ALIGN_LEFT, -1, 1},
		     {"bottom_center", OBS_ALIGN_BOTTOM, 0, 1},
		     {"bottom_right", OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT, 1, 1}};
	for (const auto &e : table) {
		if (pos != e.id)
			continue;
		t.align = e.align;
		t.x = e.hx < 0 ? m : e.hx > 0 ? w - m : cx;
		t.y = e.vy < 0 ? m : e.vy > 0 ? h - m : cy;
		return true;
	}
	return false;
}

struct AlignCtx {
	Card *card;
	float w, h;
};

bool align_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<AlignCtx *>(param);
	if (obs_sceneitem_get_source(item) != ctx->card->self)
		return true;
	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	// Внутри страницы вокруг карточки прозрачный отступ 12 px — вычитаем, чтобы отступ считался от самой карточки.
	AlignTarget t;
	const float inner = 12.0f * scale.x;
	if (!position_target(ctx->card->position, ctx->w, ctx->h, (float)ctx->card->margin - inner, t))
		return true;
	if (t.align == OBS_ALIGN_CENTER || !(t.align & (OBS_ALIGN_LEFT | OBS_ALIGN_RIGHT)))
		t.x = ctx->w / 2;
	if (t.align == OBS_ALIGN_CENTER || !(t.align & (OBS_ALIGN_TOP | OBS_ALIGN_BOTTOM)))
		t.y = ctx->h / 2;
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	if (obs_sceneitem_get_alignment(item) != t.align)
		obs_sceneitem_set_alignment(item, t.align);
	if (fabsf(pos.x - t.x) > 0.5f || fabsf(pos.y - t.y) > 0.5f) {
		vec2 np = {};
		np.x = t.x;
		np.y = t.y;
		obs_sceneitem_set_pos(item, &np);
	}
	return true;
}

bool align_in_scene(void *param, obs_source_t *scene_source)
{
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (scene)
		obs_scene_enum_items(scene, align_item, param);
	return true;
}

void align_card(Card *card)
{
	if (card->position == "manual")
		return;
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;
	AlignCtx ctx{card, (float)ovi.base_width, (float)ovi.base_height};
	obs_enum_scenes(align_in_scene, &ctx);
}

// Сцены меняем в потоке интерфейса, а не в потоке отрисовки; источник к этому моменту мог быть удалён.
void align_task(void *param)
{
	obs_weak_source_t *weak = static_cast<obs_weak_source_t *>(param);
	obs_source_t *source = obs_weak_source_get_source(weak);
	obs_weak_source_release(weak);
	if (!source)
		return;
	if (auto *card = static_cast<Card *>(obs_obj_get_data(source)))
		align_card(card);
	obs_source_release(source);
}

// Агент мог перезапуститься на другом порту — тогда переоткрываем карточку.
void card_tick(void *data, float seconds)
{
	auto *card = static_cast<Card *>(data);
	// Раз в секунду: новые элементы сцены с нашим источником и сдвинутые вручную встают на место.
	card->align_timer += seconds;
	if (card->align_timer >= 1.0f) {
		card->align_timer = 0.0f;
		if (card->position != "manual")
			obs_queue_task(OBS_TASK_UI, align_task, obs_source_get_weak_source(card->self), false);
	}
	if (runtime_base_url() != card->applied_base)
		apply_browser_settings(card);
}

// Через перечисление детей OBS активирует/показывает скрытый браузер вместе с нашим источником.
void card_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	auto *card = static_cast<Card *>(data);
	if (card->browser)
		enum_callback(card->self, card->browser, param);
}

// Своя иконка в списке источников (OBS 33+): OBS сам освобождает строку через bfree, obs_module_file отдаёт bmalloc.
const char *card_dark_icon(void *)
{
	return obs_module_file("source-icon-dark.svg");
}

const char *card_light_icon(void *)
{
	return obs_module_file("source-icon-light.svg");
}

// Поля get_dark_icon/get_light_icon появились в конце obs_source_info в OBS 33. Собираемся с заголовками 31,
// поэтому дописываем их сами и передаём больший размер только тем OBS, которые его знают.
struct SourceInfoV33 {
	obs_source_info base;
	const char *(*get_dark_icon)(void *type_data);
	const char *(*get_light_icon)(void *type_data);
};

} // namespace

void register_card_source()
{
	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	const bool custom_icons = obs_get_version() >= MAKE_SEMANTIC_VERSION(33, 0, 0);
	info.get_name = card_get_name;
	info.create = card_create;
	info.destroy = card_destroy;
	info.update = card_update;
	info.get_defaults = card_defaults;
	info.get_properties = card_properties;
	info.get_width = card_width;
	info.get_height = card_height;
	info.video_render = card_render;
	info.video_tick = card_tick;
	info.enum_active_sources = card_enum_sources;
	info.enum_all_sources = card_enum_sources;
	if (custom_icons) {
		SourceInfoV33 v33 = {};
		v33.base = info;
		v33.base.icon_type = OBS_ICON_TYPE_CUSTOM;
		v33.get_dark_icon = card_dark_icon;
		v33.get_light_icon = card_light_icon;
		obs_register_source_s(&v33.base, sizeof(v33));
	} else {
		obs_register_source(&info);
	}
}

} // namespace trackspin
