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

// Плагин не содержит логики: он запускает агент (отдельную программу рядом с DLL), следит за ним
// и пересылает ему настройки источника. Агент завершается сам, когда закрывается OBS.

#include "runtime.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <httplib.h>
#endif

namespace trackspin {

#ifdef _WIN32
namespace {

std::mutex g_mu;
PROCESS_INFORMATION g_proc = {};
std::string g_token;
std::string g_base_url;
std::string g_port_file;
std::string g_log_file;
RuntimeOptions g_opts;
std::thread g_watchdog;
std::atomic<bool> g_stopping{false};

std::string random_token()
{
	std::random_device rd; // на Windows — криптостойкий источник (rand_s)
	char buf[33];
	for (int i = 0; i < 4; ++i)
		std::snprintf(buf + i * 8, 9, "%08x", rd());
	return buf;
}

std::wstring widen(const std::string &s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

std::string agent_path()
{
	std::string dll = obs_get_module_binary_path(obs_current_module());
	size_t slash = dll.find_last_of("/\\");
	return dll.substr(0, slash + 1) + "trackspin-agent.exe";
}

// Строка JSON: кавычки и обратная косая экранируются, управляющие символы отбрасываются.
std::string json_string(const std::string &v)
{
	std::string out = "\"";
	for (unsigned char c : v) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += (char)c;
		} else if (c >= 0x20) {
			out += (char)c;
		}
	}
	return out + "\"";
}

std::string options_json(const RuntimeOptions &o)
{
	auto b = [](bool v) { return v ? "true" : "false"; };
	std::string j = "{";
	j += "\"yandex\":"; j += b(o.yandex); j += ",";
	j += "\"vk\":"; j += b(o.vk); j += ",";
	j += "\"spotify\":"; j += b(o.spotify); j += ",";
	j += "\"appleMusic\":"; j += b(o.appleMusic); j += ",";
	j += "\"youtubeMusic\":"; j += b(o.youtubeMusic); j += ",";
	j += "\"soundcloud\":"; j += b(o.soundcloud); j += ",";
	j += "\"deezer\":"; j += b(o.deezer); j += ",";
	j += "\"tidal\":"; j += b(o.tidal); j += ",";
	j += "\"amazonMusic\":"; j += b(o.amazonMusic); j += ",";
	j += "\"youtube\":"; j += b(o.youtube); j += ",";
	j += "\"otherPlayers\":"; j += b(o.otherPlayers); j += ",";
	j += "\"videoPlayers\":"; j += b(o.videoPlayers); j += ",";
	j += "\"browserWithoutExtension\":"; j += b(o.browser_without_extension); j += ",";
	j += "\"demo\":"; j += b(o.demo); j += ",";
	j += "\"blocklist\":[";
	for (size_t i = 0; i < o.blocklist.size(); ++i)
		j += (i ? "," : "") + json_string(o.blocklist[i]);
	j += "],\"outputDir\":" + json_string(o.output_dir);
	return j + "}";
}

int port_from_url(const std::string &url)
{
	size_t colon = url.rfind(':');
	return colon == std::string::npos ? 0 : std::atoi(url.c_str() + colon + 1);
}

bool post(const std::string &base, const std::string &token, const char *path, const std::string &body)
{
	if (base.empty())
		return false;
	httplib::Client cli("127.0.0.1", port_from_url(base));
	cli.set_connection_timeout(1, 0);
	cli.set_read_timeout(2, 0);
	httplib::Headers h = {{"X-Trackspin-Token", token}};
	auto res = cli.Post(path, h, body, "application/json");
	return res && res->status == 200;
}

// Запускает агент и ждёт, пока он сообщит порт. Вызывается под g_mu.
bool launch_locked()
{
	g_token = random_token();
	g_base_url.clear();
	std::remove(g_port_file.c_str());

	std::string exe = agent_path();
	std::string cmd = "\"" + exe + "\" --token " + g_token + " --port-file \"" + g_port_file + "\" --parent-pid " +
			  std::to_string(GetCurrentProcessId()) + " --log-file \"" + g_log_file + "\"";
	std::wstring wcmd = widen(cmd);
	STARTUPINFOW si = {};
	si.cb = sizeof si;
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
			    &pi)) {
		obs_log(LOG_ERROR, "cannot start agent %s (error %lu)", exe.c_str(), GetLastError());
		return false;
	}
	g_proc = pi;

	for (int i = 0; i < 100; ++i) { // до 5 секунд
		std::ifstream f(g_port_file);
		int port = 0;
		if (f >> port && port > 0) {
			g_base_url = "http://127.0.0.1:" + std::to_string(port);
			obs_log(LOG_INFO, "agent started: %s/widget (pid %lu)", g_base_url.c_str(), pi.dwProcessId);
			post(g_base_url, g_token, "/api/config", options_json(g_opts));
			return true;
		}
		if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0)
			break;
	}
	obs_log(LOG_ERROR, "agent did not report a port; see %s", g_log_file.c_str());
	return false;
}

void close_handles_locked()
{
	if (g_proc.hProcess)
		CloseHandle(g_proc.hProcess);
	if (g_proc.hThread)
		CloseHandle(g_proc.hThread);
	g_proc = {};
}

// Следит за агентом и перезапускает его при падении (не чаще 5 раз за минуту).
void watchdog()
{
	auto window_start = std::chrono::steady_clock::now();
	int restarts = 0;
	while (!g_stopping) {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		std::lock_guard<std::mutex> lk(g_mu);
		if (g_stopping)
			break;
		if (g_proc.hProcess && WaitForSingleObject(g_proc.hProcess, 0) == WAIT_TIMEOUT)
			continue;
		auto now = std::chrono::steady_clock::now();
		if (now - window_start > std::chrono::minutes(1)) {
			window_start = now;
			restarts = 0;
		}
		if (restarts >= 5)
			continue;
		++restarts;
		obs_log(LOG_WARNING, "agent is not running, restarting (%d)", restarts);
		close_handles_locked();
		launch_locked();
	}
}

} // namespace

void runtime_start()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		std::string d = dir;
		bfree(dir);
		g_port_file = d + "agent-" + std::to_string(GetCurrentProcessId()) + ".port";
		g_log_file = d + "agent.log";
	}
	g_stopping = false;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		launch_locked();
	}
	g_watchdog = std::thread(watchdog);
}

void runtime_stop()
{
	g_stopping = true;
	if (g_watchdog.joinable())
		g_watchdog.join();
	std::lock_guard<std::mutex> lk(g_mu);
	if (g_proc.hProcess) {
		post(g_base_url, g_token, "/api/shutdown", "");
		if (WaitForSingleObject(g_proc.hProcess, 2000) == WAIT_TIMEOUT)
			TerminateProcess(g_proc.hProcess, 0);
		close_handles_locked();
	}
	g_base_url.clear();
	std::remove(g_port_file.c_str());
}

void runtime_set_options(const RuntimeOptions &opts)
{
	std::string base, token, body;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		g_opts = opts;
		base = g_base_url;
		token = g_token;
		body = options_json(opts);
	}
	// Не блокируем поток OBS сетевым запросом.
	std::thread([base, token, body] { post(base, token, "/api/config", body); }).detach();
}

std::string runtime_base_url()
{
	std::lock_guard<std::mutex> lk(g_mu);
	return g_base_url;
}

std::string runtime_get(const char *path)
{
	std::string base = runtime_base_url();
	if (base.empty())
		return std::string();
	httplib::Client cli("127.0.0.1", port_from_url(base));
	cli.set_connection_timeout(0, 300000);
	cli.set_read_timeout(0, 500000);
	auto res = cli.Get(path);
	return res && res->status == 200 ? res->body : std::string();
}

#else // не Windows: агент пока только для Windows

void runtime_start() {}
void runtime_stop() {}
void runtime_set_options(const RuntimeOptions &) {}
std::string runtime_base_url()
{
	return std::string();
}
std::string runtime_get(const char *)
{
	return std::string();
}

#endif

} // namespace trackspin
