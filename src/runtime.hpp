#pragma once
// Связь с агентом (trackspin-agent.exe): запуск вместе с OBS, перезапуск при падении, передача настроек.
#include <string>
#include <vector>

namespace trackspin {

struct RuntimeOptions {
	bool yandex = true;
	bool vk = true;
	bool spotify = true;
	bool appleMusic = true;
	bool youtubeMusic = true;
	bool soundcloud = true;
	bool deezer = true;
	bool tidal = true;
	bool amazonMusic = true;
	bool youtube = false;
	bool otherPlayers = true;
	bool videoPlayers = false;
	bool browser_without_extension = true;
	bool demo = false;
	std::vector<std::string> blocklist; // «Не показывать»: исполнители и треки
	std::string output_dir;             // папка для текстовых файлов; пусто — выключено
};

void runtime_start();
void runtime_stop();
// Настройки общие; применяются из свойств источника «Сейчас играет» (последний изменённый — главный).
void runtime_set_options(const RuntimeOptions &opts);
// Адрес агента вида http://127.0.0.1:48210 или пустая строка, если агент не запущен.
std::string runtime_base_url();
// GET к агенту (для строки состояния в свойствах источника). Пустая строка — агент недоступен.
std::string runtime_get(const char *path);

} // namespace trackspin
